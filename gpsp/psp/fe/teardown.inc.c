/* ---- THE ONE EXIT SEQUENCE (Epoch C, builds/epochc-out/PLAN-EPOCHC-beta5.md) --
 * Every way out of the process before the frame loop runs ONE sequence, in the
 * order its site always had, chosen by flags that are constants at each call:
 *
 *   exit_early_begin(f)   [ME] [CORE] [AUD VID]   what goes before the site's log line
 *   (the site's own `exit code=..` line, its text untouched: the Lab parses it)
 *   exit_early_end(f,..)  ES [AUD VID] HO|RL EXIT  the log closes, the handoff, the XMB
 *
 * The default order is AV-first (AUD VID EVT ES HO EXIT: family A, and the loop's
 * own end).  XE_EVT_FIRST keeps the order the selection's exits (B1-B10, C1, C2)
 * have: EVT ES AUD VID HO EXIT.  XE_ME and XE_CORE are the bad-script / net-failed
 * pair (C3, C4): the ME first (HW-2 F9), then the writer, the core and its save.
 * XE_RELAUNCH boots again instead of handing off (B8: not an end of run).
 * XE_HO_PASS hands off 0 when `pass`, else (code, why) (A5, HW-2 F7).
 * The exit map (tools/refactor/exit_matrix.py, T8) reads every call as this
 * body with its flags applied, so each site is checked as the copy it replaced.
 * Constant `if`s here have braced arms and no `else if`: the tools fold them. */
enum
{
   XE_EVT_FIRST = 1u << 0,
   XE_ME        = 1u << 1,
   XE_CORE      = 1u << 2,
   XE_RELAUNCH  = 1u << 3,
   XE_HO_PASS   = 1u << 4
};

static inline __attribute__((always_inline)) void exit_early_begin(unsigned f)
{
   if (f & XE_ME)
   {
      /* The ME and its renderer stop before the core is freed, in the
       * loop teardown's order (F9). */
      me_rend_teardown("exit");
      me_video_teardown("exit", 1);
      me_host_shutdown();
   }
   if (f & XE_CORE)
   {
      io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */
      fe_host_shutdown();
#ifdef REWIND
      PSP_REWIND_GAME_OVER();   /* fe_host let go of the pool */
#endif
   }
   if (!(f & XE_EVT_FIRST))
   {
      audio_stop();
      vid_term();
   }
}

static inline __attribute__((always_inline)) void exit_early_end(unsigned f,
   int code, const char *why, int pass)
{
   evt_shutdown();
   if (f & XE_EVT_FIRST)
   {
      audio_stop();
      vid_term();
   }
   if (f & XE_RELAUNCH)
   {
      relaunch_eboot(0);
   }
   if (f & XE_HO_PASS)
   {
      if (pass)
         FE_HANDOFF(0, NULL);
      else
         FE_HANDOFF(code, why);   /* ADR-0053 */
   }
   if (!(f & (XE_RELAUNCH | XE_HO_PASS)))
   {
      FE_HANDOFF(code, why);      /* ADR-0053 */
   }
   FE_EXIT_GAME();
}

